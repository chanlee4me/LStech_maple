/***************************************************************************************[Solver.cc]
MiniSat -- Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
           Copyright (c) 2007-2010, Niklas Sorensson
 
Chanseok Oh's MiniSat Patch Series -- Copyright (c) 2015, Chanseok Oh
 
Maple_LCM, Based on MapleCOMSPS_DRUP -- Copyright (c) 2017, Mao Luo, Chu-Min LI, Fan Xiao: implementing a learnt clause minimisation approach
Reference: M. Luo, C.-M. Li, F. Xiao, F. Manya, and Z. L. , “An effective learnt clause minimization approach for cdcl sat solvers,” in IJCAI-2017, 2017, pp. to–appear.

Maple_LCM_Dist, Based on Maple_LCM -- Copyright (c) 2017, Fan Xiao, Chu-Min LI, Mao Luo: using a new branching heuristic called Distance at the beginning of search

MapleLCMDistChronoBT, based on Maple_LCM_Dist -- Copyright (c), Alexander Nadel, Vadim Ryvchin: "Chronological Backtracking" in SAT-2018, pp. 111-121.

MapleLCMDistChronoBT-DL, based on MapleLCMDistChronoBT -- Copyright (c), Stepan Kochemazov, Oleg Zaikin, Victor Kondratiev, Alexander Semenov: The solver was augmented with heuristic that moves duplicate learnt clauses into the core/tier2 tiers depending on a number of parameters.
 
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include <math.h>
#include <algorithm>
#include <signal.h>
#include <unistd.h>

#include "mtl/Sort.h"
#include "core/Solver.h"
#include "utils/ccnr.h"
#include "utils/System.h"
using namespace Minisat;

//#define PRINT_OUT

#ifdef BIN_DRUP
int Solver::buf_len = 0;
unsigned char Solver::drup_buf[2 * 1024 * 1024];
unsigned char* Solver::buf_ptr = drup_buf;
#endif


//=================================================================================================
// Options:


static const char* _cat = "CORE";

static DoubleOption  opt_step_size         (_cat, "step-size",   "Initial step size",                             0.40,     DoubleRange(0, false, 1, false));
static DoubleOption  opt_step_size_dec     (_cat, "step-size-dec","Step size decrement",                          0.000001, DoubleRange(0, false, 1, false));
static DoubleOption  opt_min_step_size     (_cat, "min-step-size","Minimal step size",                            0.06,     DoubleRange(0, false, 1, false));
static DoubleOption  opt_var_decay         (_cat, "var-decay",   "The variable activity decay factor",            0.80,     DoubleRange(0, false, 1, false));
static DoubleOption  opt_clause_decay      (_cat, "cla-decay",   "The clause activity decay factor",              0.999,    DoubleRange(0, false, 1, false));
static DoubleOption  opt_random_var_freq   (_cat, "rnd-freq",    "The frequency with which the decision heuristic tries to choose a random variable", 0, DoubleRange(0, true, 1, true));
static DoubleOption  opt_random_seed       (_cat, "rnd-seed",    "Used by the random variable selection",         91648253, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_ccmin_mode        (_cat, "ccmin-mode",  "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2, IntRange(0, 2));
static IntOption     opt_phase_saving      (_cat, "phase-saving", "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2, IntRange(0, 2));
static BoolOption    opt_rnd_init_act      (_cat, "rnd-init",    "Randomize the initial activity", false);
static IntOption     opt_restart_first     (_cat, "rfirst",      "The base restart interval", 100, IntRange(1, INT32_MAX));
static DoubleOption  opt_restart_inc       (_cat, "rinc",        "Restart interval increase factor", 2, DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption  opt_garbage_frac      (_cat, "gc-frac",     "The fraction of wasted memory allowed before a garbage collection is triggered",  0.20, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_chrono            (_cat, "chrono",  "Controls if to perform chrono backtrack", 100, IntRange(-1, INT32_MAX));
static IntOption     opt_conf_to_chrono    (_cat, "confl-to-chrono",  "Controls number of conflicts to perform chrono backtrack", 4000, IntRange(-1, INT32_MAX));

static IntOption     opt_max_lbd_dup       ("DUP-LEARNTS", "lbd-limit",  "specifies the maximum lbd of learnts to be screened for duplicates.", 14, IntRange(0, INT32_MAX));
static IntOption     opt_min_dupl_app      ("DUP-LEARNTS", "min-dup-app",  "specifies the minimum number of learnts to be included into db.", 2, IntRange(2, INT32_MAX));
static IntOption     opt_dupl_db_init_size ("DUP-LEARNTS", "dupdb-init",  "specifies the initial maximal duplicates DB size.", 1000000, IntRange(1, INT32_MAX));


//=================================================================================================
// Constructor/Destructor:


Solver::Solver() :

    // Parameters (user settable):
    //
    drup_file        (NULL)
  , verbosity        (0)
  , step_size        (opt_step_size)
  , step_size_dec    (opt_step_size_dec)
  , min_step_size    (opt_min_step_size)
  , timer            (5000)
  , var_decay        (opt_var_decay)
  , clause_decay     (opt_clause_decay)
  , random_var_freq  (opt_random_var_freq)
  , random_seed      (opt_random_seed)
  , VSIDS            (false)
  , ccmin_mode       (opt_ccmin_mode)
  , phase_saving     (opt_phase_saving)
  , rnd_pol          (false)
  , rnd_init_act     (opt_rnd_init_act)
  , garbage_frac     (opt_garbage_frac)
  , restart_first    (opt_restart_first)
  , restart_inc      (opt_restart_inc)


  , min_number_of_learnts_copies(opt_min_dupl_app)  
  , max_lbd_dup(opt_max_lbd_dup)
  , dupl_db_init_size(opt_dupl_db_init_size)

  // Parameters (the rest):
  //
  , learntsize_factor((double)1/(double)3), learntsize_inc(1.1)

  // Parameters (experimental):
  //
  , learntsize_adjust_start_confl (100)
  , learntsize_adjust_inc         (1.5)

  // Statistics: (formerly in 'SolverStats')
  //
  , solves(0), starts(0), decisions(0), rnd_decisions(0), propagations(0), conflicts(0), conflicts_VSIDS(0)
  , dec_vars(0), clauses_literals(0), learnts_literals(0), max_literals(0), tot_literals(0)
  , chrono_backtrack(0), non_chrono_backtrack(0)

  , ok                 (true)
  , cla_inc            (1)
  , var_inc            (1)
  , watches_bin        (WatcherDeleted(ca))
  , watches            (WatcherDeleted(ca))
  , qhead              (0)
  , simpDB_assigns     (-1)
  , simpDB_props       (0)
  , order_heap_CHB     (VarOrderLt(activity_CHB))
  , order_heap_VSIDS   (VarOrderLt(activity_VSIDS))
  , progress_estimate  (0)
  , remove_satisfied   (true)

  , core_lbd_cut       (2)
  , global_lbd_sum     (0)
  , lbd_queue          (50)
  , next_T2_reduce     (10000)
  , next_L_reduce      (15000)
  , confl_to_chrono    (opt_conf_to_chrono)
  , chrono			   (opt_chrono)
  
  , counter            (0)

  // Resource constraints:
  //
  , conflict_budget    (-1)
  , propagation_budget (-1)
  , asynch_interrupt   (false)

  // simplfiy
  , nbSimplifyAll(0)
  , s_propagations(0)

  // simplifyAll adjust occasion
  , curSimplify(1)
  , nbconfbeforesimplify(1000)
  , incSimplify(1000)

{}


Solver::~Solver()
{
}


// simplify All
//
CRef Solver::simplePropagate()
{
    CRef    confl = CRef_Undef;
    int     num_props = 0;
    watches.cleanAll();
    watches_bin.cleanAll();
    while (qhead < trail.size())
    {
        Lit            p = trail[qhead++];     // 'p' is enqueued fact to propagate.
        vec<Watcher>&  ws = watches[p];
        Watcher        *i, *j, *end;
        num_props++;


        // First, Propagate binary clauses
        vec<Watcher>&  wbin = watches_bin[p];

        for (int k = 0; k<wbin.size(); k++)
        {

            Lit imp = wbin[k].blocker;

            if (value(imp) == l_False)
            {
                return wbin[k].cref;
            }

            if (value(imp) == l_Undef)
            {
                simpleUncheckEnqueue(imp, wbin[k].cref);
            }
        }
        for (i = j = (Watcher*)ws, end = i + ws.size(); i != end;)
        {
            // Try to avoid inspecting the clause:
            Lit blocker = i->blocker;
            if (value(blocker) == l_True)
            {
                *j++ = *i++; continue;
            }

            // Make sure the false literal is data[1]:
            CRef     cr = i->cref;
            Clause&  c = ca[cr];
            Lit      false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            //  i++;

            // If 0th watch is true, then clause is already satisfied.
            // However, 0th watch is not the blocker, make it blocker using a new watcher w
            // why not simply do i->blocker=first in this case?
            Lit     first = c[0];
            //  Watcher w     = Watcher(cr, first);
            if (first != blocker && value(first) == l_True)
            {
                i->blocker = first;
                *j++ = *i++; continue;
            }

            // Look for new watch:
            //if (incremental)
            //{ // ----------------- INCREMENTAL MODE
            //	int choosenPos = -1;
            //	for (int k = 2; k < c.size(); k++)
            //	{
            //		if (value(c[k]) != l_False)
            //		{
            //			if (decisionLevel()>assumptions.size())
            //			{
            //				choosenPos = k;
            //				break;
            //			}
            //			else
            //			{
            //				choosenPos = k;

            //				if (value(c[k]) == l_True || !isSelector(var(c[k]))) {
            //					break;
            //				}
            //			}

            //		}
            //	}
            //	if (choosenPos != -1)
            //	{
            //		// watcher i is abandonned using i++, because cr watches now ~c[k] instead of p
            //		// the blocker is first in the watcher. However,
            //		// the blocker in the corresponding watcher in ~first is not c[1]
            //		Watcher w = Watcher(cr, first); i++;
            //		c[1] = c[choosenPos]; c[choosenPos] = false_lit;
            //		watches[~c[1]].push(w);
            //		goto NextClause;
            //	}
            //}
            else
            {  // ----------------- DEFAULT  MODE (NOT INCREMENTAL)
                for (int k = 2; k < c.size(); k++)
                {

                    if (value(c[k]) != l_False)
                    {
                        // watcher i is abandonned using i++, because cr watches now ~c[k] instead of p
                        // the blocker is first in the watcher. However,
                        // the blocker in the corresponding watcher in ~first is not c[1]
                        Watcher w = Watcher(cr, first); i++;
                        c[1] = c[k]; c[k] = false_lit;
                        watches[~c[1]].push(w);
                        goto NextClause;
                    }
                }
            }

            // Did not find watch -- clause is unit under assignment:
            i->blocker = first;
            *j++ = *i++;
            if (value(first) == l_False)
            {
                confl = cr;
                qhead = trail.size();
                // Copy the remaining watches:
                while (i < end)
                    *j++ = *i++;
            }
            else
            {
                simpleUncheckEnqueue(first, cr);
            }
NextClause:;
        }
        ws.shrink(i - j);
    }

    s_propagations += num_props;

    return confl;
}

void Solver::simpleUncheckEnqueue(Lit p, CRef from){
    assert(value(p) == l_Undef);
    assigns[var(p)] = lbool(!sign(p)); // this makes a lbool object whose value is sign(p)
    vardata[var(p)].reason = from;
    trail.push_(p);
}

void Solver::cancelUntilTrailRecord()
{
    for (int c = trail.size() - 1; c >= trailRecord; c--)
    {
        Var x = var(trail[c]);
        assigns[x] = l_Undef;

    }
    qhead = trailRecord;
    trail.shrink(trail.size() - trailRecord);

}

void Solver::litsEnqueue(int cutP, Clause& c)
{
    for (int i = cutP; i < c.size(); i++)
    {
        simpleUncheckEnqueue(~c[i]);
    }
}

bool Solver::removed(CRef cr) {
    return ca[cr].mark() == 1;
}

void Solver::simpleAnalyze(CRef confl, vec<Lit>& out_learnt, vec<CRef>& reason_clause, bool True_confl)
{
    int pathC = 0;
    Lit p = lit_Undef;
    int index = trail.size() - 1;

    do{
        if (confl != CRef_Undef){
            reason_clause.push(confl);
            Clause& c = ca[confl];
            // Special case for binary clauses
            // The first one has to be SAT
            if (p != lit_Undef && c.size() == 2 && value(c[0]) == l_False) {

                assert(value(c[1]) == l_True);
                Lit tmp = c[0];
                c[0] = c[1], c[1] = tmp;
            }
            // if True_confl==true, then choose p begin with the 1th index of c;
            for (int j = (p == lit_Undef && True_confl == false) ? 0 : 1; j < c.size(); j++){
                Lit q = c[j];
                if (!seen[var(q)]){
                    seen[var(q)] = 1;
                    pathC++;
                }
            }
        }
        else if (confl == CRef_Undef){
            out_learnt.push(~p);
        }
        // if not break, while() will come to the index of trail blow 0, and fatal error occur;
        if (pathC == 0) break;
        // Select next clause to look at:
        while (!seen[var(trail[index--])]);
        // if the reason cr from the 0-level assigned var, we must break avoid move forth further;
        // but attention that maybe seen[x]=1 and never be clear. However makes no matter;
        if (trailRecord > index + 1) break;
        p = trail[index + 1];
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;

    } while (pathC >= 0);
}

void Solver::simplifyLearnt(Clause& c)
{
    ////
    original_length_record += c.size();

    trailRecord = trail.size();// record the start pointer

    vec<Lit> falseLit;
    falseLit.clear();

    //sort(&c[0], c.size(), VarOrderLevelLt(vardata));

    bool True_confl = false;
    int beforeSize, afterSize;
    beforeSize = c.size();
    int i, j;
    CRef confl;

    for (i = 0, j = 0; i < c.size(); i++){
        if (value(c[i]) == l_Undef){
            //printf("///@@@ uncheckedEnqueue:index = %d. l_Undef\n", i);
            simpleUncheckEnqueue(~c[i]);
            c[j++] = c[i];
            confl = simplePropagate();
            if (confl != CRef_Undef){
                break;
            }
        }
        else{
            if (value(c[i]) == l_True){
                //printf("///@@@ uncheckedEnqueue:index = %d. l_True\n", i);
                c[j++] = c[i];
                True_confl = true;
                confl = reason(var(c[i]));
                break;
            }
            else{
                //printf("///@@@ uncheckedEnqueue:index = %d. l_False\n", i);
                falseLit.push(c[i]);
            }
        }
    }
    c.shrink(c.size() - j);
    afterSize = c.size();
    //printf("\nbefore : %d, after : %d ", beforeSize, afterSize);


    if (confl != CRef_Undef || True_confl == true){
        simp_learnt_clause.clear();
        simp_reason_clause.clear();
        if (True_confl == true){
            simp_learnt_clause.push(c.last());
        }
        simpleAnalyze(confl, simp_learnt_clause, simp_reason_clause, True_confl);

        if (simp_learnt_clause.size() < c.size()){
            for (i = 0; i < simp_learnt_clause.size(); i++){
                c[i] = simp_learnt_clause[i];
            }
            c.shrink(c.size() - i);
        }
    }

    cancelUntilTrailRecord();

    ////
    simplified_length_record += c.size();

}

bool Solver::simplifyLearnt_x(vec<CRef>& learnts_x)
{
    int beforeSize, afterSize;
    int learnts_x_size_before = learnts_x.size();

    int ci, cj, li, lj;
    bool sat, false_lit;
    unsigned int nblevels;
    ////
    //printf("learnts_x size : %d\n", learnts_x.size());

    ////
    int nbSimplified = 0;
    int nbSimplifing = 0;

    for (ci = 0, cj = 0; ci < learnts_x.size(); ci++){
        CRef cr = learnts_x[ci];
        Clause& c = ca[cr];

        if (removed(cr)) continue;
        else if (c.simplified()){
            learnts_x[cj++] = learnts_x[ci];
            ////
            nbSimplified++;
        }
        else{
            ////
            nbSimplifing++;
            sat = false_lit = false;
            for (int i = 0; i < c.size(); i++){
                if (value(c[i]) == l_True){
                    sat = true;
                    break;
                }
                else if (value(c[i]) == l_False){
                    false_lit = true;
                }
            }
            if (sat){
                removeClause(cr);
            }
            else{
                detachClause(cr, true);

                if (false_lit){
                    for (li = lj = 0; li < c.size(); li++){
                        if (value(c[li]) != l_False){
                            c[lj++] = c[li];
                        }
                    }
                    c.shrink(li - lj);
                }

                beforeSize = c.size();
                assert(c.size() > 1);
                // simplify a learnt clause c
                simplifyLearnt(c);
                assert(c.size() > 0);
                afterSize = c.size();

                //printf("beforeSize: %2d, afterSize: %2d\n", beforeSize, afterSize);

                if (c.size() == 1){
                    // when unit clause occur, enqueue and propagate
                    uncheckedEnqueue(c[0]);
                    if (propagate() != CRef_Undef){
                        ok = false;
                        return false;
                    }
                    // delete the clause memory in logic
                    c.mark(1);
                    ca.free(cr);
                }
                else{
                    attachClause(cr);
                    learnts_x[cj++] = learnts_x[ci];

                    nblevels = computeLBD(c);
                    if (nblevels < c.lbd()){
                        //printf("lbd-before: %d, lbd-after: %d\n", c.lbd(), nblevels);
                        c.set_lbd(nblevels);
                    }
                    if (c.mark() != CORE){
                        if (c.lbd() <= core_lbd_cut){
                            //if (c.mark() == LOCAL) local_learnts_dirty = true;
                            //else tier2_learnts_dirty = true;
                            cj--;
                            learnts_core.push(cr);
                            c.mark(CORE);
                        }
                        else if (c.mark() == LOCAL && c.lbd() <= 6){
                            //local_learnts_dirty = true;
                            cj--;
                            learnts_tier2.push(cr);
                            c.mark(TIER2);
                        }
                    }

                    c.setSimplified(true);
                }
            }
        }
    }
    learnts_x.shrink(ci - cj);

    //   printf("c nbLearnts_x %d / %d, nbSimplified: %d, nbSimplifing: %d\n",
    //          learnts_x_size_before, learnts_x.size(), nbSimplified, nbSimplifing);

    return true;
}

bool Solver::simplifyLearnt_core()
{
    int beforeSize, afterSize;
    int learnts_core_size_before = learnts_core.size();

    int ci, cj, li, lj;
    bool sat, false_lit;
    unsigned int nblevels;
    ////
    //printf("learnts_x size : %d\n", learnts_x.size());

    ////
    int nbSimplified = 0;
    int nbSimplifing = 0;

    for (ci = 0, cj = 0; ci < learnts_core.size(); ci++){
        CRef cr = learnts_core[ci];
        Clause& c = ca[cr];

        if (removed(cr)) continue;
        else if (c.simplified()){
            learnts_core[cj++] = learnts_core[ci];
            ////
            nbSimplified++;
        }
        else{
            int saved_size=c.size();
            //         if (drup_file){
            //                 add_oc.clear();
            //                 for (int i = 0; i < c.size(); i++) add_oc.push(c[i]); }
            ////
            nbSimplifing++;
            sat = false_lit = false;
            for (int i = 0; i < c.size(); i++){
                if (value(c[i]) == l_True){
                    sat = true;
                    break;
                }
                else if (value(c[i]) == l_False){
                    false_lit = true;
                }
            }
            if (sat){
                removeClause(cr);
            }
            else{
                detachClause(cr, true);

                if (false_lit){
                    for (li = lj = 0; li < c.size(); li++){
                        if (value(c[li]) != l_False){
                            c[lj++] = c[li];
                        }
                    }
                    c.shrink(li - lj);
                }

                beforeSize = c.size();
                assert(c.size() > 1);
                // simplify a learnt clause c
                simplifyLearnt(c);
                assert(c.size() > 0);
                afterSize = c.size();
                
                if(drup_file && saved_size !=c.size()){
#ifdef BIN_DRUP
                    binDRUP('a', c , drup_file);
                    //                    binDRUP('d', add_oc, drup_file);
#else
                    for (int i = 0; i < c.size(); i++)
                        fprintf(drup_file, "%i ", (var(c[i]) + 1) * (-2 * sign(c[i]) + 1));
                    fprintf(drup_file, "0\n");

                    //                    fprintf(drup_file, "d ");
                    //                    for (int i = 0; i < add_oc.size(); i++)
                    //                        fprintf(drup_file, "%i ", (var(add_oc[i]) + 1) * (-2 * sign(add_oc[i]) + 1));
                    //                    fprintf(drup_file, "0\n");
#endif
                }

                //printf("beforeSize: %2d, afterSize: %2d\n", beforeSize, afterSize);

                if (c.size() == 1){
                    // when unit clause occur, enqueue and propagate
                    uncheckedEnqueue(c[0]);
                    if (propagate() != CRef_Undef){
                        ok = false;
                        return false;
                    }
                    // delete the clause memory in logic
                    c.mark(1);
                    ca.free(cr);
//#ifdef BIN_DRUP
//                    binDRUP('d', c, drup_file);
//#else
//                    fprintf(drup_file, "d ");
//                    for (int i = 0; i < c.size(); i++)
//                        fprintf(drup_file, "%i ", (var(c[i]) + 1) * (-2 * sign(c[i]) + 1));
//                    fprintf(drup_file, "0\n");
//#endif
                }
                else{
                    attachClause(cr);
                    learnts_core[cj++] = learnts_core[ci];

                    nblevels = computeLBD(c);
                    if (nblevels < c.lbd()){
                        //printf("lbd-before: %d, lbd-after: %d\n", c.lbd(), nblevels);
                        c.set_lbd(nblevels);
                    }

                    c.setSimplified(true);
                }
            }
        }
    }
    learnts_core.shrink(ci - cj);

    //    printf("c nbLearnts_core %d / %d, nbSimplified: %d, nbSimplifing: %d\n",
    //           learnts_core_size_before, learnts_core.size(), nbSimplified, nbSimplifing);

    return true;

}


int Solver::is_duplicate(std::vector<uint32_t>&c){
   auto time_point_0 = std::chrono::high_resolution_clock::now();
    dupl_db_size++;
    int res = 0;    
    
    int sz = c.size();
    std::vector<uint32_t> tmp(c);    
    sort(tmp.begin(),tmp.end());
    
    uint64_t hash = 0;    
    
    for (int i =0; i<sz; i++) {
        hash ^= tmp[i] + 0x9e3779b9 + (hash << 6) + (hash>> 2);     
    }    
    
    int32_t head = tmp[0];
    auto it0 = ht.find(head);
    if (it0 != ht.end()){
        auto it1=ht[head].find(sz);
        if (it1 != ht[head].end()){
            auto it2 = ht[head][sz].find(hash);
            if (it2 != ht[head][sz].end()){
                it2->second++;
                res = it2->second;            
            }
            else{
                ht[head][sz][hash]=1;
            }
        }
        else{            
            ht[head][sz][hash]=1;
        }
    }else{        
        ht[head][sz][hash]=1;
    } 
    auto time_point_1 = std::chrono::high_resolution_clock::now();
    duptime += std::chrono::duration_cast<std::chrono::microseconds>(time_point_1-time_point_0);    
    return res;
}

bool Solver::simplifyLearnt_tier2()
{
    int beforeSize, afterSize;
    int learnts_tier2_size_before = learnts_tier2.size();

    int ci, cj, li, lj;
    bool sat, false_lit;
    unsigned int nblevels;
    ////
    //printf("learnts_x size : %d\n", learnts_x.size());

    ////
    int nbSimplified = 0;
    int nbSimplifing = 0;

    for (ci = 0, cj = 0; ci < learnts_tier2.size(); ci++){
        CRef cr = learnts_tier2[ci];
        Clause& c = ca[cr];

        if (removed(cr)) continue;
        else if (c.simplified()){
            learnts_tier2[cj++] = learnts_tier2[ci];
            ////
            nbSimplified++;
        }
        else{
            int saved_size=c.size();
            //            if (drup_file){
            //                    add_oc.clear();
            //                    for (int i = 0; i < c.size(); i++) add_oc.push(c[i]); }
            ////
            nbSimplifing++;
            sat = false_lit = false;
            for (int i = 0; i < c.size(); i++){
                if (value(c[i]) == l_True){
                    sat = true;
                    break;
                }
                else if (value(c[i]) == l_False){
                    false_lit = true;
                }
            }
            if (sat){
                removeClause(cr);
            }
            else{
                detachClause(cr, true);

                if (false_lit){
                    for (li = lj = 0; li < c.size(); li++){
                        if (value(c[li]) != l_False){
                            c[lj++] = c[li];
                        }
                    }
                    c.shrink(li - lj);
                }

                beforeSize = c.size();
                assert(c.size() > 1);
                // simplify a learnt clause c
                simplifyLearnt(c);
                assert(c.size() > 0);
                afterSize = c.size();
                
                if(drup_file && saved_size!=c.size()){

#ifdef BIN_DRUP
                    binDRUP('a', c , drup_file);
                    //                    binDRUP('d', add_oc, drup_file);
#else
                    for (int i = 0; i < c.size(); i++)
                        fprintf(drup_file, "%i ", (var(c[i]) + 1) * (-2 * sign(c[i]) + 1));
                    fprintf(drup_file, "0\n");

                    //                    fprintf(drup_file, "d ");
                    //                    for (int i = 0; i < add_oc.size(); i++)
                    //                        fprintf(drup_file, "%i ", (var(add_oc[i]) + 1) * (-2 * sign(add_oc[i]) + 1));
                    //                    fprintf(drup_file, "0\n");
#endif
                }

                //printf("beforeSize: %2d, afterSize: %2d\n", beforeSize, afterSize);

                if (c.size() == 1){
                    // when unit clause occur, enqueue and propagate
                    uncheckedEnqueue(c[0]);
                    if (propagate() != CRef_Undef){
                        ok = false;
                        return false;
                    }
                    // delete the clause memory in logic
                    c.mark(1);
                    ca.free(cr);
//#ifdef BIN_DRUP
//                    binDRUP('d', c, drup_file);
//#else
//                    fprintf(drup_file, "d ");
//                    for (int i = 0; i < c.size(); i++)
//                        fprintf(drup_file, "%i ", (var(c[i]) + 1) * (-2 * sign(c[i]) + 1));
//                    fprintf(drup_file, "0\n");
//#endif
                }
                else{
                    

                    nblevels = computeLBD(c);
                    if (nblevels < c.lbd()){
                        //printf("lbd-before: %d, lbd-after: %d\n", c.lbd(), nblevels);
                        c.set_lbd(nblevels);
                    }
                     //duplicate learnts 
                    int id = 0;                    
                    
                    std::vector<uint32_t> tmp;
                    for (int i = 0; i < c.size(); i++)                           
                        tmp.push_back(c[i].x);
                    id = is_duplicate(tmp);
                     
                                        
                    //duplicate learnts 

                    if (id < min_number_of_learnts_copies+2){
                        attachClause(cr);
                        learnts_tier2[cj++] = learnts_tier2[ci];                    
                        if (id == min_number_of_learnts_copies+1){                            
                            duplicates_added_minimization++;                                  
                        }
                        if ((c.lbd() <= core_lbd_cut)||(id == min_number_of_learnts_copies+1)){
                        //if (id == min_number_of_learnts_copies+1){
                            cj--;
                            learnts_core.push(cr);
                            c.mark(CORE);
                        }

                        c.setSimplified(true);
                    }
                }
            }
        }
    }
    learnts_tier2.shrink(ci - cj);

    //    printf("c nbLearnts_tier2 %d / %d, nbSimplified: %d, nbSimplifing: %d\n",
    //           learnts_tier2_size_before, learnts_tier2.size(), nbSimplified, nbSimplifing);

    return true;

}

bool Solver::simplifyAll()
{
    ////
    simplified_length_record = original_length_record = 0;

    if (!ok || propagate() != CRef_Undef)
        return ok = false;

    //// cleanLearnts(also can delete these code), here just for analyzing
    //if (local_learnts_dirty) cleanLearnts(learnts_local, LOCAL);
    //if (tier2_learnts_dirty) cleanLearnts(learnts_tier2, TIER2);
    //local_learnts_dirty = tier2_learnts_dirty = false;

    if (!simplifyLearnt_core()) return ok = false;
    if (!simplifyLearnt_tier2()) return ok = false;
    //if (!simplifyLearnt_x(learnts_local)) return ok = false;

    checkGarbage();

    ////
    //  printf("c size_reduce_ratio     : %4.2f%%\n",
    //         original_length_record == 0 ? 0 : (original_length_record - simplified_length_record) * 100 / (double)original_length_record);

    return true;
}
//=================================================================================================
// Minor methods:


// Creates a new SAT variable in the solver. If 'decision' is cleared, variable will not be
// used as a decision variable (NOTE! This has effects on the meaning of a SATISFIABLE result).
//
Var Solver::newVar(bool sign, bool dvar)
{
    int v = nVars();
    //added by cl
    htab.push(0);
    //end
    watches_bin.init(mkLit(v, false));
    watches_bin.init(mkLit(v, true ));
    watches  .init(mkLit(v, false));
    watches  .init(mkLit(v, true ));
    assigns  .push(l_Undef);
    vardata  .push(mkVarData(CRef_Undef, 0));
    activity_CHB  .push(0);
    activity_VSIDS.push(rnd_init_act ? drand(random_seed) * 0.00001 : 0);

    picked.push(0);
    conflicted.push(0);

    almost_conflicted.push(0);
#ifdef ANTI_EXPLORATION
    canceled.push(0);
#endif

    seen     .push(0);
    seen2    .push(0);
    polarity .push(sign);
    decision .push();
    trail    .capacity(v+1);
    setDecisionVar(v, dvar);

    return v;
}


bool Solver::addClause_(vec<Lit>& ps)
{
    assert(decisionLevel() == 0);
    if (!ok) return false;

    // Check if clause is satisfied and remove false/duplicate literals:
    sort(ps);
    Lit p; int i, j;

    if (drup_file){
        add_oc.clear();
        for (int i = 0; i < ps.size(); i++) add_oc.push(ps[i]); }

    for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
        if (value(ps[i]) == l_True || ps[i] == ~p)
            return true;
        else if (value(ps[i]) != l_False && ps[i] != p)
            ps[j++] = p = ps[i];
    ps.shrink(i - j);

    if (drup_file && i != j){
#ifdef BIN_DRUP
        binDRUP('a', ps, drup_file);
        binDRUP('d', add_oc, drup_file);
#else
        for (int i = 0; i < ps.size(); i++)
            fprintf(drup_file, "%i ", (var(ps[i]) + 1) * (-2 * sign(ps[i]) + 1));
        fprintf(drup_file, "0\n");

        fprintf(drup_file, "d ");
        for (int i = 0; i < add_oc.size(); i++)
            fprintf(drup_file, "%i ", (var(add_oc[i]) + 1) * (-2 * sign(add_oc[i]) + 1));
        fprintf(drup_file, "0\n");
#endif
    }

    if (ps.size() == 0)
        return ok = false;
    else if (ps.size() == 1){
        uncheckedEnqueue(ps[0]);
        return ok = (propagate() == CRef_Undef);
    }else{
        CRef cr = ca.alloc(ps, false);
        clauses.push(cr);
        attachClause(cr);
    }

    return true;
}


void Solver::attachClause(CRef cr) {
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    OccLists<Lit, vec<Watcher>, WatcherDeleted>& ws = c.size() == 2 ? watches_bin : watches;
    ws[~c[0]].push(Watcher(cr, c[1]));
    ws[~c[1]].push(Watcher(cr, c[0]));
    if (c.learnt()) learnts_literals += c.size();
    else            clauses_literals += c.size(); }


void Solver::detachClause(CRef cr, bool strict) {
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    OccLists<Lit, vec<Watcher>, WatcherDeleted>& ws = c.size() == 2 ? watches_bin : watches;
    
    if (strict){
        remove(ws[~c[0]], Watcher(cr, c[1]));
        remove(ws[~c[1]], Watcher(cr, c[0]));
    }else{
        // Lazy detaching: (NOTE! Must clean all watcher lists before garbage collecting this clause)
        ws.smudge(~c[0]);
        ws.smudge(~c[1]);
    }

    if (c.learnt()) learnts_literals -= c.size();
    else            clauses_literals -= c.size(); }


void Solver::removeClause(CRef cr) {
    Clause& c = ca[cr];

    if (drup_file){
        if (c.mark() != 1){
#ifdef BIN_DRUP
            binDRUP('d', c, drup_file);
#else
            fprintf(drup_file, "d ");
            for (int i = 0; i < c.size(); i++)
                fprintf(drup_file, "%i ", (var(c[i]) + 1) * (-2 * sign(c[i]) + 1));
            fprintf(drup_file, "0\n");
#endif
        }else
            printf("c Bug. I don't expect this to happen.\n");
    }

    detachClause(cr);
    // Don't leave pointers to free'd memory!
    if (locked(c)){
        Lit implied = c.size() != 2 ? c[0] : (value(c[0]) == l_True ? c[0] : c[1]);
        vardata[var(implied)].reason = CRef_Undef; }
    c.mark(1);
    ca.free(cr);
}


bool Solver::satisfied(const Clause& c) const {
    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) == l_True)
            return true;
    return false; }


// Revert to the state at given level (keeping all assignment at 'level' but not beyond).
//
void Solver::cancelUntil(int bLevel) {
	
    if (decisionLevel() > bLevel){
#ifdef PRINT_OUT
		std::cout << "bt " << bLevel << "\n";
#endif				
		add_tmp.clear();
        for (int c = trail.size()-1; c >= trail_lim[bLevel]; c--)
        {
            Var      x  = var(trail[c]);

			if (level(x) <= bLevel)
			{
				add_tmp.push(trail[c]);
			}
			else
			{
				 if (!VSIDS){
					uint32_t age = conflicts - picked[x];
					if (age > 0){
						double adjusted_reward = ((double) (conflicted[x] + almost_conflicted[x])) / ((double) age);
						double old_activity = activity_CHB[x];
						activity_CHB[x] = step_size * adjusted_reward + ((1 - step_size) * old_activity);
						if (order_heap_CHB.inHeap(x)){
							if (activity_CHB[x] > old_activity)
								order_heap_CHB.decrease(x);
							else
								order_heap_CHB.increase(x);
						}
					}
#ifdef ANTI_EXPLORATION
					canceled[x] = conflicts;
#endif
				}
				
				assigns [x] = l_Undef;
#ifdef PRINT_OUT
				std::cout << "undo " << x << "\n";
#endif				
	            if (phase_saving > 1 || (phase_saving == 1) && c > trail_lim.last()){
					polarity[x] = sign(trail[c]);
                }
				insertVarOrder(x);
			}
        }
        qhead = trail_lim[bLevel];
        trail.shrink(trail.size() - trail_lim[bLevel]);
        trail_lim.shrink(trail_lim.size() - bLevel);
        for (int nLitId = add_tmp.size() - 1; nLitId >= 0; --nLitId)
		{
			trail.push_(add_tmp[nLitId]);
		}
		
		add_tmp.clear();
    } }


//=================================================================================================
// Major methods:


Lit Solver::pickBranchLit()
{
    Var next = var_Undef;
    //    Heap<VarOrderLt>& order_heap = VSIDS ? order_heap_VSIDS : order_heap_CHB;
    Heap<VarOrderLt>& order_heap = ((!VSIDS)? order_heap_CHB:order_heap_VSIDS);

    // Random decision:
    /*if (drand(random_seed) < random_var_freq && !order_heap.empty()){
        next = order_heap[irand(random_seed,order_heap.size())];
        if (value(next) == l_Undef && decision[next])
            rnd_decisions++; }*/

    // Activity based decision:
    while (next == var_Undef || value(next) != l_Undef || !decision[next])
        if (order_heap.empty())
            return lit_Undef;
        else{
#ifdef ANTI_EXPLORATION
            if (!VSIDS){
                Var v = order_heap_CHB[0];
                uint32_t age = conflicts - canceled[v];
                while (age > 0){
                    double decay = pow(0.95, age);
                    activity_CHB[v] *= decay;
                    if (order_heap_CHB.inHeap(v))
                        order_heap_CHB.increase(v);
                    canceled[v] = conflicts;
                    v = order_heap_CHB[0];
                    age = conflicts - canceled[v];
                }
            }
#endif
            next = order_heap.removeMin();
        }
    
    return mkLit(next, polarity[next]);
    
    
}

inline Solver::ConflictData Solver::FindConflictLevel(CRef cind)
{
	ConflictData data;
	Clause& conflCls = ca[cind];
	data.nHighestLevel = level(var(conflCls[0]));
	if (data.nHighestLevel == decisionLevel() && level(var(conflCls[1])) == decisionLevel())
	{
		return data;
	}

	int highestId = 0;
    data.bOnlyOneLitFromHighest = true;
	// find the largest decision level in the clause
	for (int nLitId = 1; nLitId < conflCls.size(); ++nLitId)
	{
		int nLevel = level(var(conflCls[nLitId]));
		if (nLevel > data.nHighestLevel)
		{
			highestId = nLitId;
			data.nHighestLevel = nLevel;
			data.bOnlyOneLitFromHighest = true;
		}
		else if (nLevel == data.nHighestLevel && data.bOnlyOneLitFromHighest == true)
		{
			data.bOnlyOneLitFromHighest = false;
		}
	}

	if (highestId != 0)
	{
		std::swap(conflCls[0], conflCls[highestId]);
		if (highestId > 1)
		{
			OccLists<Lit, vec<Watcher>, WatcherDeleted>& ws = conflCls.size() == 2 ? watches_bin : watches;
			//ws.smudge(~conflCls[highestId]);
			remove(ws[~conflCls[highestId]], Watcher(cind, conflCls[1]));
			ws[~conflCls[0]].push(Watcher(cind, conflCls[1]));
		}
	}

	return data;
}


/*_________________________________________________________________________________________________
|
|  analyze : (confl : Clause*) (out_learnt : vec<Lit>&) (out_btlevel : int&)  ->  [void]
|  
|  Description:
|    Analyze conflict and produce a reason clause.
|  
|    Pre-conditions:
|      * 'out_learnt' is assumed to be cleared.
|      * Current decision level must be greater than root level.
|  
|    Post-conditions:
|      * 'out_learnt[0]' is the asserting literal at level 'out_btlevel'.
|      * If out_learnt.size() > 1 then 'out_learnt[1]' has the greatest decision level of the 
|        rest of literals. There may be others from the same level though.
|  
|________________________________________________________________________________________________@*/
void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel, int& out_lbd)
{
    int pathC = 0;
    Lit p     = lit_Undef;

    // Generate conflict clause:
    //
    out_learnt.push();      // (leave room for the asserting literal)
    int index   = trail.size() - 1;
    int nDecisionLevel = level(var(ca[confl][0]));
    assert(nDecisionLevel == level(var(ca[confl][0])));

    do{
        assert(confl != CRef_Undef); // (otherwise should be UIP)
        Clause& c = ca[confl];
/*-----------------------addey by cl-----------------------*/
        //更新下冲突子句中每个变量的出现频率
        for(int i = 0; i < c.size(); i++)
        {   //此处c[i]是Lit类型
            htab[var(c[i])]++;
        }
/*----------------------- ----end--------------------------*/
        // For binary clauses, we don't rearrange literals in propagate(), so check and make sure the first is an implied lit.
        if (p != lit_Undef && c.size() == 2 && value(c[0]) == l_False){
            assert(value(c[1]) == l_True);
            Lit tmp = c[0];
            c[0] = c[1], c[1] = tmp; }

        // Update LBD if improved.
        if (c.learnt() && c.mark() != CORE){
            int lbd = computeLBD(c);
            /*-----------------------addey by cl-----------------------*/
            //注释：会用到的数据结构
            glue = lbd;
            /*----------------------- ----end--------------------------*/

            if (lbd < c.lbd()){
                if (c.lbd() <= 30) c.removable(false); // Protect once from reduction.
                c.set_lbd(lbd);
                if (lbd <= core_lbd_cut){
                    learnts_core.push(confl);
                    c.mark(CORE);
                }else if (lbd <= 6 && c.mark() == LOCAL){
                    // Bug: 'cr' may already be in 'learnts_tier2', e.g., if 'cr' was demoted from TIER2
                    // to LOCAL previously and if that 'cr' is not cleaned from 'learnts_tier2' yet.
                    learnts_tier2.push(confl);
                    c.mark(TIER2); }
            }

            if (c.mark() == TIER2)
                c.touched() = conflicts;
            else if (c.mark() == LOCAL)
                claBumpActivity(c);
        }

        for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++){
            Lit q = c[j];

            if (!seen[var(q)] && level(var(q)) > 0){
                if (VSIDS){
                    //added by cl
                    glue = 0;
                    //end
                    varBumpActivity(var(q), .5);
                    add_tmp.push(q);
                }else
                    conflicted[var(q)]++;
                seen[var(q)] = 1;
                if (level(var(q)) >= nDecisionLevel){
                    pathC++;
                }else
                    out_learnt.push(q);
            }
        }
        
        // Select next clause to look at:
		do {
			while (!seen[var(trail[index--])]);
			p  = trail[index+1];
		} while (level(var(p)) < nDecisionLevel);
		
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;

    }while (pathC > 0);
    out_learnt[0] = ~p;

    // Simplify conflict clause:
    //
    int i, j;
    out_learnt.copyTo(analyze_toclear);
    if (ccmin_mode == 2){
        uint32_t abstract_level = 0;
        for (i = 1; i < out_learnt.size(); i++)
            abstract_level |= abstractLevel(var(out_learnt[i])); // (maintain an abstraction of levels involved in conflict)

        for (i = j = 1; i < out_learnt.size(); i++)
            if (reason(var(out_learnt[i])) == CRef_Undef || !litRedundant(out_learnt[i], abstract_level))
                out_learnt[j++] = out_learnt[i];
        
    }else if (ccmin_mode == 1){
        for (i = j = 1; i < out_learnt.size(); i++){
            Var x = var(out_learnt[i]);

            if (reason(x) == CRef_Undef)
                out_learnt[j++] = out_learnt[i];
            else{
                Clause& c = ca[reason(var(out_learnt[i]))];
                for (int k = c.size() == 2 ? 0 : 1; k < c.size(); k++)
                    if (!seen[var(c[k])] && level(var(c[k])) > 0){
                        out_learnt[j++] = out_learnt[i];
                        break; }
            }
        }
    }else
        i = j = out_learnt.size();

    max_literals += out_learnt.size();
    out_learnt.shrink(i - j);
    tot_literals += out_learnt.size();

    out_lbd = computeLBD(out_learnt);
    if (out_lbd <= 6 && out_learnt.size() <= 30) // Try further minimization?
        if (binResMinimize(out_learnt))
            out_lbd = computeLBD(out_learnt); // Recompute LBD if minimized.

    // Find correct backtrack level:
    //
    if (out_learnt.size() == 1)
        out_btlevel = 0;
    else{
        int max_i = 1;
        // Find the first literal assigned at the next-highest level:
        for (int i = 2; i < out_learnt.size(); i++)
            if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
                max_i = i;
        // Swap-in this literal at index 1:
        Lit p             = out_learnt[max_i];
        out_learnt[max_i] = out_learnt[1];
        out_learnt[1]     = p;
        out_btlevel       = level(var(p));
    }
    //added by cl
    if(VSIDS){
        for (int i = 0; i < out_learnt.size(); i++){
            Var v = var(out_learnt[i]);
            glue = computeLBD(out_learnt);
            varBumpActivity(v, 1, glue);
        }
        add_tmp.clear();
    }
    //end
    if (VSIDS){
        for (int i = 0; i < add_tmp.size(); i++){
            Var v = var(add_tmp[i]);
            if (level(v) >= out_btlevel - 1){
                glue = 0;
                varBumpActivity(v, 1);
            }
        }
        add_tmp.clear();
    }else{
        seen[var(p)] = true;
        for(int i = out_learnt.size() - 1; i >= 0; i--){
            Var v = var(out_learnt[i]);
            CRef rea = reason(v);
            if (rea != CRef_Undef){
                const Clause& reaC = ca[rea];
                for (int i = 0; i < reaC.size(); i++){
                    Lit l = reaC[i];
                    if (!seen[var(l)]){
                        seen[var(l)] = true;
                        almost_conflicted[var(l)]++;
                        analyze_toclear.push(l); } } } } }

    for (int j = 0; j < analyze_toclear.size(); j++) seen[var(analyze_toclear[j])] = 0;    // ('seen[]' is now cleared)
}


// Try further learnt clause minimization by means of binary clause resolution.
bool Solver::binResMinimize(vec<Lit>& out_learnt)
{
    // Preparation: remember which false variables we have in 'out_learnt'.
    counter++;
    for (int i = 1; i < out_learnt.size(); i++)
        seen2[var(out_learnt[i])] = counter;

    // Get the list of binary clauses containing 'out_learnt[0]'.
    const vec<Watcher>& ws = watches_bin[~out_learnt[0]];

    int to_remove = 0;
    for (int i = 0; i < ws.size(); i++){
        Lit the_other = ws[i].blocker;
        // Does 'the_other' appear negatively in 'out_learnt'?
        if (seen2[var(the_other)] == counter && value(the_other) == l_True){
            to_remove++;
            seen2[var(the_other)] = counter - 1; // Remember to remove this variable.
        }
    }

    // Shrink.
    if (to_remove > 0){
        int last = out_learnt.size() - 1;
        for (int i = 1; i < out_learnt.size() - to_remove; i++)
            if (seen2[var(out_learnt[i])] != counter)
                out_learnt[i--] = out_learnt[last--];
        out_learnt.shrink(to_remove);
    }
    return to_remove != 0;
}


// Check if 'p' can be removed. 'abstract_levels' is used to abort early if the algorithm is
// visiting literals at levels that cannot be removed later.
bool Solver::litRedundant(Lit p, uint32_t abstract_levels)
{
    analyze_stack.clear(); analyze_stack.push(p);
    int top = analyze_toclear.size();
    while (analyze_stack.size() > 0){
        assert(reason(var(analyze_stack.last())) != CRef_Undef);
        Clause& c = ca[reason(var(analyze_stack.last()))]; analyze_stack.pop();

        // Special handling for binary clauses like in 'analyze()'.
        if (c.size() == 2 && value(c[0]) == l_False){
            assert(value(c[1]) == l_True);
            Lit tmp = c[0];
            c[0] = c[1], c[1] = tmp; }

        for (int i = 1; i < c.size(); i++){
            Lit p  = c[i];
            if (!seen[var(p)] && level(var(p)) > 0){
                if (reason(var(p)) != CRef_Undef && (abstractLevel(var(p)) & abstract_levels) != 0){
                    seen[var(p)] = 1;
                    analyze_stack.push(p);
                    analyze_toclear.push(p);
                }else{
                    for (int j = top; j < analyze_toclear.size(); j++)
                        seen[var(analyze_toclear[j])] = 0;
                    analyze_toclear.shrink(analyze_toclear.size() - top);
                    return false;
                }
            }
        }
    }

    return true;
}


/*_________________________________________________________________________________________________
|
|  analyzeFinal : (p : Lit)  ->  [void]
|  
|  Description:
|    Specialized analysis procedure to express the final conflict in terms of assumptions.
|    Calculates the (possibly empty) set of assumptions that led to the assignment of 'p', and
|    stores the result in 'out_conflict'.
|________________________________________________________________________________________________@*/
void Solver::analyzeFinal(Lit p, vec<Lit>& out_conflict)
{
    out_conflict.clear();
    out_conflict.push(p);

    if (decisionLevel() == 0)
        return;

    seen[var(p)] = 1;

    for (int i = trail.size()-1; i >= trail_lim[0]; i--){
        Var x = var(trail[i]);
        if (seen[x]){
            if (reason(x) == CRef_Undef){
                assert(level(x) > 0);
                out_conflict.push(~trail[i]);
            }else{
                Clause& c = ca[reason(x)];
                for (int j = c.size() == 2 ? 0 : 1; j < c.size(); j++)
                    if (level(var(c[j])) > 0)
                        seen[var(c[j])] = 1;
            }
            seen[x] = 0;
        }
    }

    seen[var(p)] = 0;
}


void Solver::uncheckedEnqueue(Lit p, int level, CRef from)
{
    assert(value(p) == l_Undef);
    Var x = var(p);
    if (!VSIDS){
        picked[x] = conflicts;
        conflicted[x] = 0;
        almost_conflicted[x] = 0;
#ifdef ANTI_EXPLORATION
        uint32_t age = conflicts - canceled[var(p)];
        if (age > 0){
            double decay = pow(0.95, age);
            activity_CHB[var(p)] *= decay;
            if (order_heap_CHB.inHeap(var(p)))
                order_heap_CHB.increase(var(p));
        }
#endif
    }

    assigns[x] = lbool(!sign(p));
    vardata[x] = mkVarData(from, level);
    trail.push_(p);
}


/*_________________________________________________________________________________________________
|
|  propagate : [void]  ->  [Clause*]
|  
|  Description:
|    Propagates all enqueued facts. If a conflict arises, the conflicting clause is returned,
|    otherwise CRef_Undef.
|  
|    Post-conditions:
|      * the propagation queue is empty, even if there was a conflict.
|________________________________________________________________________________________________@*/
CRef Solver::propagate()
{
    CRef    confl     = CRef_Undef;
    int     num_props = 0;
    watches.cleanAll();
    watches_bin.cleanAll();

    while (qhead < trail.size()){
        Lit            p   = trail[qhead++];     // 'p' is enqueued fact to propagate.
        int currLevel = level(var(p));
        vec<Watcher>&  ws  = watches[p];
        Watcher        *i, *j, *end;
        num_props++;

        vec<Watcher>& ws_bin = watches_bin[p];  // Propagate binary clauses first.
        for (int k = 0; k < ws_bin.size(); k++){
            Lit the_other = ws_bin[k].blocker;
            if (value(the_other) == l_False){
                confl = ws_bin[k].cref;
#ifdef LOOSE_PROP_STAT
                return confl;
#else
                goto ExitProp;
#endif
            }else if(value(the_other) == l_Undef)
            {
                uncheckedEnqueue(the_other, currLevel, ws_bin[k].cref);
#ifdef  PRINT_OUT                
                std::cout << "i " << the_other << " l " << currLevel << "\n";
#endif                
			}
        }

        for (i = j = (Watcher*)ws, end = i + ws.size();  i != end;){
            // Try to avoid inspecting the clause:
            Lit blocker = i->blocker;
            if (value(blocker) == l_True){
                *j++ = *i++; continue; }

            // Make sure the false literal is data[1]:
            CRef     cr        = i->cref;
            Clause&  c         = ca[cr];
            Lit      false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            i++;

            // If 0th watch is true, then clause is already satisfied.
            Lit     first = c[0];
            Watcher w     = Watcher(cr, first);
            if (first != blocker && value(first) == l_True){
                *j++ = w; continue; }

            // Look for new watch:
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) != l_False){
                    c[1] = c[k]; c[k] = false_lit;
                    watches[~c[1]].push(w);
                    goto NextClause; }

            // Did not find watch -- clause is unit under assignment:
            *j++ = w;
            if (value(first) == l_False){
                confl = cr;
                qhead = trail.size();
                // Copy the remaining watches:
                while (i < end)
                    *j++ = *i++;
            }else
            {
				if (currLevel == decisionLevel())
				{
					uncheckedEnqueue(first, currLevel, cr);
#ifdef PRINT_OUT					
					std::cout << "i " << first << " l " << currLevel << "\n";
#endif					
				}
				else
				{
					int nMaxLevel = currLevel;
					int nMaxInd = 1;
					// pass over all the literals in the clause and find the one with the biggest level
					for (int nInd = 2; nInd < c.size(); ++nInd)
					{
						int nLevel = level(var(c[nInd]));
						if (nLevel > nMaxLevel)
						{
							nMaxLevel = nLevel;
							nMaxInd = nInd;
						}
					}

					if (nMaxInd != 1)
					{
						std::swap(c[1], c[nMaxInd]);
						*j--; // undo last watch
						watches[~c[1]].push(w);
					}
					
					uncheckedEnqueue(first, nMaxLevel, cr);
#ifdef PRINT_OUT					
					std::cout << "i " << first << " l " << nMaxLevel << "\n";
#endif	
				}
			}

NextClause:;
        }
        ws.shrink(i - j);
    }

ExitProp:;
    propagations += num_props;
    simpDB_props -= num_props;

    return confl;
}


/*_________________________________________________________________________________________________
|
|  reduceDB : ()  ->  [void]
|  
|  Description:
|    Remove half of the learnt clauses, minus the clauses locked by the current assignment. Locked
|    clauses are clauses that are reason to some assignment. Binary clauses are never removed.
|________________________________________________________________________________________________@*/
struct reduceDB_lt { 
    ClauseAllocator& ca;
    reduceDB_lt(ClauseAllocator& ca_) : ca(ca_) {}
    bool operator () (CRef x, CRef y) const { return ca[x].activity() < ca[y].activity(); }
};
void Solver::reduceDB()
{
    int     i, j;
    //if (local_learnts_dirty) cleanLearnts(learnts_local, LOCAL);
    //local_learnts_dirty = false;

    sort(learnts_local, reduceDB_lt(ca));

    int limit = learnts_local.size() / 2;
    for (i = j = 0; i < learnts_local.size(); i++){
        Clause& c = ca[learnts_local[i]];
        if (c.mark() == LOCAL)
            if (c.removable() && !locked(c) && i < limit)
                removeClause(learnts_local[i]);
            else{
                if (!c.removable()) limit++;
                c.removable(true);
                learnts_local[j++] = learnts_local[i]; }
    }
    learnts_local.shrink(i - j);

    checkGarbage();
}
void Solver::reduceDB_Tier2()
{
    int i, j;
    for (i = j = 0; i < learnts_tier2.size(); i++){
        Clause& c = ca[learnts_tier2[i]];
        if (c.mark() == TIER2)
            if (!locked(c) && c.touched() + 30000 < conflicts){
                learnts_local.push(learnts_tier2[i]);
                c.mark(LOCAL);
                //c.removable(true);
                c.activity() = 0;
                claBumpActivity(c);
            }else
                learnts_tier2[j++] = learnts_tier2[i];
    }
    learnts_tier2.shrink(i - j);
}


void Solver::removeSatisfied(vec<CRef>& cs)
{
    int i, j;
    for (i = j = 0; i < cs.size(); i++){
        Clause& c = ca[cs[i]];
        if (satisfied(c))
            removeClause(cs[i]);
        else
            cs[j++] = cs[i];
    }
    cs.shrink(i - j);
}

void Solver::safeRemoveSatisfied(vec<CRef>& cs, unsigned valid_mark)
{
    int i, j;
    for (i = j = 0; i < cs.size(); i++){
        Clause& c = ca[cs[i]];
        if (c.mark() == valid_mark)
            if (satisfied(c))
                removeClause(cs[i]);
            else
                cs[j++] = cs[i];
    }
    cs.shrink(i - j);
}

void Solver::rebuildOrderHeap()
{
    vec<Var> vs;
    for (Var v = 0; v < nVars(); v++)
        if (decision[v] && value(v) == l_Undef)
            vs.push(v);

    order_heap_CHB  .build(vs);
    order_heap_VSIDS.build(vs);
}


/*_________________________________________________________________________________________________
|
|  simplify : [void]  ->  [bool]
|  
|  Description:
|    Simplify the clause database according to the current top-level assigment. Currently, the only
|    thing done here is the removal of satisfied clauses, but more things can be put here.
|________________________________________________________________________________________________@*/
bool Solver::simplify()
{
    assert(decisionLevel() == 0);

    if (!ok || propagate() != CRef_Undef)
        return ok = false;

    if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
        return true;

    // Remove satisfied clauses:
    removeSatisfied(learnts_core); // Should clean core first.
    safeRemoveSatisfied(learnts_tier2, TIER2);
    safeRemoveSatisfied(learnts_local, LOCAL);
    if (remove_satisfied)        // Can be turned off.
        removeSatisfied(clauses);
    checkGarbage();
    rebuildOrderHeap();

    simpDB_assigns = nAssigns();
    simpDB_props   = clauses_literals + learnts_literals;   // (shouldn't depend on stats really, but it will do for now)

    return true;
}

/*_________________________________________________________________________________________________
|
|  search : (nof_conflicts : int) (params : const SearchParams&)  ->  [lbool]
|  
|  Description:
|    Search for a model the specified number of conflicts. 
|  
|  Output:
|    'l_True' if a partial assigment that is consistent with respect to the clauseset is found. If
|    all variables are decision variables, this means that the clause set is satisfiable. 'l_False'
|    if the clause set is unsatisfiable. 'l_Undef' if the bound on number of conflicts is reached.
|________________________________________________________________________________________________@*/

void Solver::info_based_rephase(){
    int var_nums = nVars();
    for(int i=0;i<var_nums;++i) polarity[i] = !ls_mediation_soln[i];
    
    for(int i=0;i<lssolver->in_conflict_sz;++i){
        int v = lssolver->in_conflict[i];
        if(VSIDS){
            varBumpActivity(v-1,lssolver->conflict_ct[v]);
        }else{
            conflicted[v-1] += lssolver->conflict_ct[v];
        }
    }
    
        
    
}

void Solver::rand_based_rephase(){
        int var_nums  = nVars();
        int pick_rand = rand()%1000;

        //local search
        if     ((pick_rand-=100)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = !ls_best_soln[i];
        }else if((pick_rand-=300)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = !ls_mediation_soln[i];
            // mediation_used = true;
        }
        //top_trail 200
        else if((pick_rand-=300)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = !top_trail_soln[i];
        }
        //reverse
        else if((pick_rand-=50)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = !polarity[i];
        }else if((pick_rand-=25)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = ls_best_soln[i];
        }else if((pick_rand-=25)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = top_trail_soln[i];
        }
        //150
        else if((pick_rand-=140)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = rand()%2==0?1:0;
        }
        else if((pick_rand-=5)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = 1;
        }else if((pick_rand-=5)<0){
            for(int i=0;i<var_nums;++i) polarity[i] = 0;
        }
        //50
        else{
            //do nothing 
        }
}


lbool Solver::search(int& nof_conflicts)
{
    assert(ok);
    int         backtrack_level;
    int         lbd;
    vec<Lit>    learnt_clause;
    bool        cached = false;
    starts++;

    freeze_ls_restart_num--;
    bool    can_call_ls = true;

    if(freeze_ls_restart_num<1){

        bool res = call_ls(top_trail_UP);

        if(res){
            solved_by_ls = true;
            return l_True;
        }
            
    }

    // if(ls_call_num>1){
        if(rand()%100<50) info_based_rephase();
        else rand_based_rephase();
    // }

    

    // simplify
    //
    if (conflicts >= curSimplify * nbconfbeforesimplify){
        //        printf("c ### simplifyAll on conflict : %lld\n", conflicts);
        //printf("nbClauses: %d, nbLearnts_core: %d, nbLearnts_tier2: %d, nbLearnts_local: %d, nbLearnts: %d\n",
        //	clauses.size(), learnts_core.size(), learnts_tier2.size(), learnts_local.size(),
        //	learnts_core.size() + learnts_tier2.size() + learnts_local.size());
        nbSimplifyAll++;
        if (!simplifyAll()){
            return l_False;
        }
        curSimplify = (conflicts / nbconfbeforesimplify) + 1;
        nbconfbeforesimplify += incSimplify;
    }

    for (;;){
        CRef confl = propagate();


        if (confl != CRef_Undef){
            // CONFLICT
            if (VSIDS){
                if (--timer == 0 && var_decay < 0.95) timer = 5000, var_decay += 0.01;
            }else
                if (step_size > min_step_size) step_size -= step_size_dec;

            conflicts++; nof_conflicts--;
            //if (conflicts == 100000 && learnts_core.size() < 100) core_lbd_cut = 5;
            ConflictData data = FindConflictLevel(confl);
            if (data.nHighestLevel == 0) return l_False;
            if (data.bOnlyOneLitFromHighest)
            {
				cancelUntil(data.nHighestLevel - 1);
				continue;
			}
			
            learnt_clause.clear();
            analyze(confl, learnt_clause, backtrack_level, lbd);
            // check chrono backtrack condition
            if ((confl_to_chrono < 0 || confl_to_chrono <= conflicts) && chrono > -1 && (decisionLevel() - backtrack_level) >= chrono)
            {
				++chrono_backtrack;
				cancelUntil(data.nHighestLevel -1);
			}
			else // default behavior
			{
				++non_chrono_backtrack;
				cancelUntil(backtrack_level);
			}

            lbd--;
            if (VSIDS){
                cached = false;
                conflicts_VSIDS++;
                lbd_queue.push(lbd);
                global_lbd_sum += (lbd > 50 ? 50 : lbd); }

            if (learnt_clause.size() == 1){
                uncheckedEnqueue(learnt_clause[0]);
            }else{
                CRef cr = ca.alloc(learnt_clause, true);
                ca[cr].set_lbd(lbd);
                //duplicate learnts 
                int  id = 0;
                if (lbd <= max_lbd_dup){                        
                    std::vector<uint32_t> tmp;
                    for (int i = 0; i < learnt_clause.size(); i++)
                        tmp.push_back(learnt_clause[i].x);
                    id = is_duplicate(tmp);             
                    if (id == min_number_of_learnts_copies +1){
                        duplicates_added_conflicts++;                        
                    }                    
                    if (id == min_number_of_learnts_copies){
                        duplicates_added_tier2++;
                    }                                        
                }
                //duplicate learnts

                if ((lbd <= core_lbd_cut) || (id == min_number_of_learnts_copies+1)){
                    learnts_core.push(cr);
                    ca[cr].mark(CORE);
                }else if ((lbd <= 6)||(id == min_number_of_learnts_copies)){
                    learnts_tier2.push(cr);
                    ca[cr].mark(TIER2);
                    ca[cr].touched() = conflicts;
                }else{
                    learnts_local.push(cr);
                    claBumpActivity(ca[cr]); }
                attachClause(cr);

                uncheckedEnqueue(learnt_clause[0], backtrack_level, cr);
#ifdef PRINT_OUT
                std::cout << "new " << ca[cr] << "\n";
                std::cout << "ci " << learnt_clause[0] << " l " << backtrack_level << "\n";
#endif                
            }
            if (drup_file){
#ifdef BIN_DRUP
                binDRUP('a', learnt_clause, drup_file);
#else
                for (int i = 0; i < learnt_clause.size(); i++)
                    fprintf(drup_file, "%i ", (var(learnt_clause[i]) + 1) * (-2 * sign(learnt_clause[i]) + 1));
                fprintf(drup_file, "0\n");
#endif
            }

            if (VSIDS) varDecayActivity();
            claDecayActivity();

            /*if (--learntsize_adjust_cnt == 0){
                learntsize_adjust_confl *= learntsize_adjust_inc;
                learntsize_adjust_cnt    = (int)learntsize_adjust_confl;
                max_learnts             *= learntsize_inc;

                if (verbosity >= 1)
                    printf("c | %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n",
                           (int)conflicts,
                           (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]), nClauses(), (int)clauses_literals,
                           (int)max_learnts, nLearnts(), (double)learnts_literals/nLearnts(), progressEstimate()*100);
            }*/

        // the top_trail_soln should be update after each conflict
        if(trail.size() > max_trail){
            max_trail = trail.size();
            build_soln_with_UP();
            int var_nums = nVars();
            for(int i=0;i<var_nums;++i){top_trail_soln[i]=tmp_up_build_soln[i];}
            // for(int idx_i=0; idx_i<var_nums; ++idx_i){
            //     lbool value_i = value(idx_i);
            //     if(value_i==l_Undef) top_trail_soln[idx_i] = !polarity[idx_i];
            //     else{
            //         top_trail_soln[idx_i] = value_i==l_True?1:0;
            //     }
            // }

        }

        }else{

            // NO_CONFLICT
                
            // if( can_call_ls && freeze_ls_restart_num < 1 && mediation_used   \
            //     && (trail.size() > (int)(conflict_ratio * nVars()) || trail.size() > (int)(percent_ratio * max_trail) )\
            //     //&& up_time_ratio * search_start_cpu_time > ls_used_time 
            //     ){
            
            //     can_call_ls     = false;
            //     mediation_used  = false;
            //     freeze_ls_restart_num = restarts_gap;
            //     bool res = call_ls(current_UP);

            //     if(res){
            //         solved_by_ls = true;
            //         return l_True;
            //     }
            // }

            

            bool restart = false;
            if (!VSIDS)
                restart = nof_conflicts <= 0;
            else if (!cached){
                restart = lbd_queue.full() && (lbd_queue.avg() * 0.8 > global_lbd_sum / conflicts_VSIDS);
                cached = true;
            }
            if (restart /*|| !withinBudget()*/){
                lbd_queue.clear();
                cached = false;
                // Reached bound on number of conflicts:
                progress_estimate = progressEstimate();
                cancelUntil(0);
                return l_Undef; }

            // Simplify the set of problem clauses:
            if (decisionLevel() == 0 && !simplify())
                return l_False;

            if (conflicts >= next_T2_reduce){
                next_T2_reduce = conflicts + 10000;
                reduceDB_Tier2(); }
            if (conflicts >= next_L_reduce){
                next_L_reduce = conflicts + 15000;
                reduceDB(); }

            Lit next = lit_Undef;
            /*while (decisionLevel() < assumptions.size()){
                // Perform user provided assumption:
                Lit p = assumptions[decisionLevel()];
                if (value(p) == l_True){
                    // Dummy decision level:
                    newDecisionLevel();
                }else if (value(p) == l_False){
                    analyzeFinal(~p, conflict);
                    return l_False;
                }else{
                    next = p;
                    break;
                }
            }

            if (next == lit_Undef)*/{
                // New variable decision:
                decisions++;
                next = pickBranchLit();

                if (next == lit_Undef)
                    // Model found:
                    return l_True;
            }

            // Increase decision level and enqueue 'next'
            newDecisionLevel();
            uncheckedEnqueue(next, decisionLevel());
#ifdef PRINT_OUT            
            std::cout << "d " << next << " l " << decisionLevel() << "\n";
#endif            
        }
    }
}


double Solver::progressEstimate() const
{
    double  progress = 0;
    double  F = 1.0 / nVars();

    for (int i = 0; i <= decisionLevel(); i++){
        int beg = i == 0 ? 0 : trail_lim[i - 1];
        int end = i == decisionLevel() ? trail.size() : trail_lim[i];
        progress += pow(F, i) * (end - beg);
    }

    return progress / nVars();
}

/*
  Finite subsequences of the Luby-sequence:

  0: 1
  1: 1 1 2
  2: 1 1 2 1 1 2 4
  3: 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8
  ...


 */

static double luby(double y, int x){

    // Find the finite subsequence that contains index 'x', and the
    // size of that subsequence:
    int size, seq;
    for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);

    while (size-1 != x){
        size = (size-1)>>1;
        seq--;
        x = x % size;
    }

    return pow(y, seq);
}

static bool switch_mode = false;
static void SIGALRM_switch(int signum) { switch_mode = true; }

// NOTE: assumptions passed in member-variable 'assumptions'.
lbool Solver::solve_()
{
    // signal(SIGALRM, SIGALRM_switch);
    // alarm(2500);

    model.clear();
    conflict.clear();
    if (!ok) return l_False;

    solves++;

    max_learnts               = nClauses() * learntsize_factor;
    learntsize_adjust_confl   = learntsize_adjust_start_confl;
    learntsize_adjust_cnt     = (int)learntsize_adjust_confl;
    lbool   status            = l_Undef;


    ls_mediation_soln = new char[nVars()+2];
    ls_best_soln      = new char[nVars()+2];
    top_trail_soln    = new char[nVars()+2];
    tmp_up_build_soln = new char[nVars()+2];

    if (verbosity >= 1){
        printf("c ============================[ Search Statistics ]==============================\n");
        printf("c | Conflicts |          ORIGINAL         |          LEARNT          | Progress |\n");
        printf("c |           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl |          |\n");
        printf("c ===============================================================================\n");
    }

    add_tmp.clear();
    
    lssolver = (CCAnr*)malloc(sizeof(CCAnr)*1);
    int fls_res = call_ls(current_UP);
    if(fls_res){
        status = l_True;
    }
    for(int i=0;i<nVars();++i) top_trail_soln[i] = ls_best_soln[i];

    VSIDS = true;
    int init = 10000;
    while (status == l_Undef && init > 0 /*&& withinBudget()*/)
        status = search(init);
    VSIDS = false;

    duplicates_added_conflicts = 0;
    duplicates_added_minimization=0;
    duplicates_added_tier2 =0;    

    dupl_db_size=0;
    size_t dupl_db_size_limit = dupl_db_init_size;

    // Search:
    int curr_restarts = 0;
    last_switch_conflicts = starts;
    while (status == l_Undef /*&& withinBudget()*/){
        if (dupl_db_size >= dupl_db_size_limit){    
            printf("c Duplicate learnts added (Minimization) %i.\n",duplicates_added_minimization);    
            printf("c Duplicate learnts added (conflicts) %i.\n",duplicates_added_conflicts);    
            printf("c Duplicate learnts added (tier2) %i.\n",duplicates_added_tier2);    
            printf("c Duptime: %i\n",duptime.count());
            printf("c Number of conflicts: %i\n",conflicts);
            printf("c Core size: %i\n",learnts_core.size());
            
            uint32_t removed_duplicates = 0;
            std::vector<std::vector<uint64_t>> tmp;
            //std::map<int32_t,std::map<uint32_t,std::unordered_map<uint64_t,uint32_t>>>  ht;
            for (auto & outer_mp: ht){//variables
                for (auto &inner_mp:outer_mp.second){//sizes
                    for (auto &in_in_mp: inner_mp.second){
                        if (in_in_mp.second >= min_number_of_learnts_copies){
                            tmp.push_back({outer_mp.first,inner_mp.first,in_in_mp.first,in_in_mp.second});
                        }
                    }                    
                 }
            }          
            removed_duplicates = dupl_db_size-tmp.size();  
            ht.clear();
            for (auto i=0;i<tmp.size();i++){
                ht[tmp[i][0]][tmp[i][1]][tmp[i][2]]=tmp[i][3];
            }
            //ht_old.clear();
            dupl_db_size_limit*=1.1;
            dupl_db_size -= removed_duplicates;
            printf("c removed duplicate db entries %i\n",removed_duplicates);
        }        
        if (VSIDS){
            int weighted = INT32_MAX;
            status = search(weighted);
        }else{
            int nof_conflicts = luby(restart_inc, curr_restarts) * restart_first;
            curr_restarts++;
            status = search(nof_conflicts);
        }
        if(starts-last_switch_conflicts > switch_heristic_mod){
            if(VSIDS){
                VSIDS = false;
            }else{
                VSIDS = true;
//                 picked.clear();
//                 conflicted.clear();
//                 almost_conflicted.clear();
// #ifdef ANTI_EXPLORATION
//                 canceled.clear();
// #endif
            }
            last_switch_conflicts = starts;
//            cout<<"c Swith"<<VSIDS<<endl;
        }

//         if (!VSIDS && switch_mode){
//             VSIDS = true;
//             printf("c Switched to VSIDS.\n");
//             fflush(stdout);
//             picked.clear();
//             conflicted.clear();
//             almost_conflicted.clear();
// #ifdef ANTI_EXPLORATION
//             canceled.clear();
// #endif
//         }
    }

    if (verbosity >= 1)
        printf("c ===============================================================================\n");

#ifdef BIN_DRUP
    if (drup_file && status == l_False) binDRUP_flush(drup_file);
#endif

    if (status == l_True){
        // Extend & copy model:
        model.growTo(nVars());
        if(solved_by_ls)
            for (int i = 0; i < nVars(); i++) model[i] = ls_mediation_soln[i]?l_True:l_False;
        else
            for (int i = 0; i < nVars(); i++) model[i] = value(i);
        
    }else if (status == l_False && conflict.size() == 0)
        ok = false;

    cancelUntil(0);
    delete [] ls_mediation_soln;
    delete [] ls_best_soln;
    delete [] top_trail_soln;
    delete [] tmp_up_build_soln;
    free(lssolver);
    return status;
}

//=================================================================================================
// Writing CNF to DIMACS:
// 
// FIXME: this needs to be rewritten completely.

static Var mapVar(Var x, vec<Var>& map, Var& max)
{
    if (map.size() <= x || map[x] == -1){
        map.growTo(x+1, -1);
        map[x] = max++;
    }
    return map[x];
}


void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max)
{
    if (satisfied(c)) return;

    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) != l_False)
            fprintf(f, "%s%d ", sign(c[i]) ? "-" : "", mapVar(var(c[i]), map, max)+1);
    fprintf(f, "0\n");
}


void Solver::toDimacs(const char *file, const vec<Lit>& assumps)
{
    FILE* f = fopen(file, "wr");
    if (f == NULL)
        fprintf(stderr, "could not open file %s\n", file), exit(1);
    toDimacs(f, assumps);
    fclose(f);
}


void Solver::toDimacs(FILE* f, const vec<Lit>& assumps)
{
    // Handle case when solver is in contradictory state:
    if (!ok){
        fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
        return; }

    vec<Var> map; Var max = 0;

    // Cannot use removeClauses here because it is not safe
    // to deallocate them at this point. Could be improved.
    int cnt = 0;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]]))
            cnt++;

    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]])){
            Clause& c = ca[clauses[i]];
            for (int j = 0; j < c.size(); j++)
                if (value(c[j]) != l_False)
                    mapVar(var(c[j]), map, max);
        }

    // Assumptions are added as unit clauses:
    cnt += assumptions.size();

    fprintf(f, "p cnf %d %d\n", max, cnt);

    for (int i = 0; i < assumptions.size(); i++){
        assert(value(assumptions[i]) != l_False);
        fprintf(f, "%s%d 0\n", sign(assumptions[i]) ? "-" : "", mapVar(var(assumptions[i]), map, max)+1);
    }

    for (int i = 0; i < clauses.size(); i++)
        toDimacs(f, ca[clauses[i]], map, max);

    if (verbosity > 0)
        printf("c Wrote %d clauses with %d variables.\n", cnt, max);
}


//=================================================================================================
// Garbage Collection methods:

void Solver::relocAll(ClauseAllocator& to)
{
    // All watchers:
    //
    // for (int i = 0; i < watches.size(); i++)
    watches.cleanAll();
    watches_bin.cleanAll();
    for (int v = 0; v < nVars(); v++)
        for (int s = 0; s < 2; s++){
            Lit p = mkLit(v, s);
            // printf(" >>> RELOCING: %s%d\n", sign(p)?"-":"", var(p)+1);
            vec<Watcher>& ws = watches[p];
            for (int j = 0; j < ws.size(); j++)
                ca.reloc(ws[j].cref, to);
            vec<Watcher>& ws_bin = watches_bin[p];
            for (int j = 0; j < ws_bin.size(); j++)
                ca.reloc(ws_bin[j].cref, to);
        }

    // All reasons:
    //
    for (int i = 0; i < trail.size(); i++){
        Var v = var(trail[i]);

        if (reason(v) != CRef_Undef && (ca[reason(v)].reloced() || locked(ca[reason(v)])))
            ca.reloc(vardata[v].reason, to);
    }

    // All learnt:
    //
    for (int i = 0; i < learnts_core.size(); i++)
        ca.reloc(learnts_core[i], to);
    for (int i = 0; i < learnts_tier2.size(); i++)
        ca.reloc(learnts_tier2[i], to);
    for (int i = 0; i < learnts_local.size(); i++)
        ca.reloc(learnts_local[i], to);

    // All original:
    //
    int i, j;
    for (i = j = 0; i < clauses.size(); i++)
        if (ca[clauses[i]].mark() != 1){
            ca.reloc(clauses[i], to);
            clauses[j++] = clauses[i]; }
    clauses.shrink(i - j);
}


void Solver::garbageCollect()
{
    // Initialize the next region to a size corresponding to the estimated utilization degree. This
    // is not precise but should avoid some unnecessary reallocations for the new region:
    ClauseAllocator to(ca.size() - ca.wasted());

    relocAll(to);
    if (verbosity >= 2)
        printf("c |  Garbage collection:   %12d bytes => %12d bytes             |\n",
               ca.size()*ClauseAllocator::Unit_Size, to.size()*ClauseAllocator::Unit_Size);
    to.moveTo(ca);
}




void Solver::load_ls_data(){
    using namespace std;
    int ls_var_nums = nVars();
    int ls_cls_nums = nClauses()+learnts_core.size()+learnts_tier2.size();
    int ls_trail_sz;
    if(trail_lim.size()>0) ls_trail_sz = trail_lim[0];
    else ls_trail_sz = trail.size();
    ls_cls_nums += ls_trail_sz;
    lssolver->num_vars = ls_var_nums;
    lssolver->num_clauses = ls_cls_nums;
    lssolver->mems_left = (long long) ls_mems_num;
    lssolver->ratio = (lssolver->num_clauses+0.0)/lssolver->num_vars;
    alloc_memory(lssolver);
    for (int c = 0; c < lssolver->num_clauses; c++) 
		lssolver->clause_lit_count[c] = lssolver->clause_delete[c] = 0;
	for (int v=1; v<=lssolver->num_vars; ++v)
		lssolver->var_lit_count[v] = lssolver->fix[v] = 0;
	lssolver->max_clause_len = 0;
	lssolver->min_clause_len = lssolver->num_vars;
    
    using namespace std;
    int cls_ct = 0;
    for(int idx=0;idx<3;++idx){
        vec<CRef> &vs = (idx==0)?clauses:(idx==1?learnts_core:(idx==2?learnts_tier2:learnts_local));
        int vs_sz = vs.size();
        for(int j=0;j<vs_sz;j++){
            CRef &cr = vs[j];
            Clause &c = ca[cr];
            lssolver->clause_lit_count[cls_ct] = c.size();
            lssolver->clause_lit[cls_ct] = (lit*)malloc(sizeof(lit)*(lssolver->clause_lit_count[cls_ct]+1));
            int i=0;
            for(;i<lssolver->clause_lit_count[cls_ct];i++){
                int cur_lit = toFormal(c[i]);
                lssolver->clause_lit[cls_ct][i].clause_num = cls_ct;
                lssolver->clause_lit[cls_ct][i].var_num = abs(cur_lit);
                if (cur_lit > 0) lssolver->clause_lit[cls_ct][i].sense = 1;
                else    lssolver->clause_lit[cls_ct][i].sense = 0;
                lssolver->var_lit_count[lssolver->clause_lit[cls_ct][i].var_num]++;
            }
            lssolver->clause_lit[cls_ct][i].var_num=0; 
		    lssolver->clause_lit[cls_ct][i].clause_num = -1;
            //unit clause
            if(lssolver->clause_lit_count[cls_ct]==1){
                lssolver->unitclause_queue[lssolver->unitclause_queue_end_pointer++] = lssolver->clause_lit[cls_ct][0];
                lssolver->clause_delete[cls_ct]=1;
            }
            if(lssolver->clause_lit_count[cls_ct] > lssolver->max_clause_len)
                lssolver->max_clause_len = lssolver->clause_lit_count[cls_ct];
            else if(lssolver->clause_lit_count[cls_ct] < lssolver->min_clause_len)
                lssolver->min_clause_len = lssolver->clause_lit_count[cls_ct];
            lssolver->formula_len += lssolver->clause_lit_count[cls_ct];

            cls_ct++;
        }
    }
    for(int j=0;j<ls_trail_sz;++j){
        int cur_lit = toFormal(trail[j]);
        lssolver->clause_lit_count[cls_ct] = 1;
        lssolver->clause_lit[cls_ct] = (lit*)malloc(sizeof(lit)*(2));
        lssolver->clause_lit[cls_ct][0].clause_num = cls_ct;
        lssolver->clause_lit[cls_ct][0].var_num = abs(cur_lit);
        if (cur_lit > 0) lssolver->clause_lit[cls_ct][0].sense = 1;
        else    lssolver->clause_lit[cls_ct][0].sense = 0;
        lssolver->var_lit_count[lssolver->clause_lit[cls_ct][0].var_num]++;
        lssolver->clause_lit[cls_ct][1].var_num=0; 
        lssolver->clause_lit[cls_ct][1].clause_num=-1;
        lssolver->unitclause_queue[lssolver->unitclause_queue_end_pointer++] = lssolver->clause_lit[cls_ct][0];
        lssolver->clause_delete[cls_ct]=1;
        if(lssolver->min_clause_len > 1)lssolver->min_clause_len = 1;
        lssolver->formula_len += 1;
        cls_ct++;
    }
    update_after_build(lssolver);
    
}

void Solver::build_soln_with_UP(){
    #include <vector>
    using std::vector;
    double start_time = cpuTime();
    int var_nums = nVars();
    int trl_sz   = trail.size();
    int trl_loc  = qhead;
    Lit* trl = new Lit[var_nums+1];
    int  undef_vars_sz = 0;
    int* undef_vars = new int[var_nums+1];
    int* idx_in_undef_vars = new int[var_nums+1];
    // -1 undef, 0 false, 1 true
    for(int i=0;i<trl_sz;++i) trl[i]=trail[i];
    for(int i=0;i<var_nums;++i) {
        if(value(i) == l_Undef){
            undef_vars[undef_vars_sz] = i;
            idx_in_undef_vars[i] = undef_vars_sz++;
            tmp_up_build_soln[i] = -1;
        }else{
            tmp_up_build_soln[i] = (value(i) == l_True) ? 1 : 0;
        }
    }

    // do UP
    while(undef_vars_sz>0){
        while(trl_loc<trl_sz && undef_vars_sz>0){
            Lit &p = trl[trl_loc++];

            // watch binary codes
            vec<Watcher>& ws_bin = watches_bin[p];
            int ws_bin_sz = ws_bin.size();
            for(int k=0;k<ws_bin_sz;++k){
                Lit the_other = ws_bin[k].blocker;
                Var the_other_var = var(the_other);
                if(tmp_up_build_soln[the_other_var]==-1){
                    tmp_up_build_soln[the_other_var] = sign(the_other)?0:1;
                    trl[trl_sz++] = the_other;

                    int end_var = undef_vars[--undef_vars_sz];
                    int idx_end_var = idx_in_undef_vars[the_other_var];
                    undef_vars[idx_end_var] = end_var;
                    idx_in_undef_vars[end_var] = idx_end_var;
                    if(undef_vars_sz==0) break;
                }
            }
            if(undef_vars_sz==0) break;

            // not binary codes
            vec<Watcher>& ws = watches[p];
            Watcher *i,*j,*end;
            for(i=j=(Watcher*)ws,end=i+ws.size();i!=end;){
                Lit blocker = i->blocker;
                // the clause is already SAT
                if(tmp_up_build_soln[var(blocker)]==!sign(blocker)){*j++ = *i++;continue;}

                CRef   cr   = i->cref;
                Clause& c    = ca[cr];
                
                // make sure c[1] is the false_lit to swap 
                Lit    false_lit = ~p;
                if(c[0]==false_lit) c[0]=c[1],c[1]=false_lit;
                i++;

                // judge if the first lit is l_true
                Lit first = c[0];
                Var first_var = var(first);
                Watcher w = Watcher(cr,first);
                if(first!=blocker && tmp_up_build_soln[first_var]==!sign(first)){*j++ = w; continue;}
                
                // first is l_false or l_undef 
                // then find another !l_false lit 
                int c_sz = c.size();
                bool should_goto_next_cls = false;
                for(int k=2;k<c_sz;++k){
                    Lit tmp_lit = c[k];
                    Var tmp_var = var(tmp_lit);
                    if(tmp_up_build_soln[tmp_var]!=sign(tmp_lit)){
                        c[1] = c[k];
                        c[k] = false_lit;
                        watches[~c[1]].push(w);
                        should_goto_next_cls=true;break;
                    }
                }if(should_goto_next_cls) continue;
                

                // all lit except c[0] in this cls is l_false.
                *j++ = w;
                if(tmp_up_build_soln[first_var]==-1){ // is unit cls
                    tmp_up_build_soln[first_var] = !sign(first);
                    trl[trl_sz++] = mkLit(first_var,sign(first));
                    int end_var = undef_vars[--undef_vars_sz];
                    int idx_end_var = idx_in_undef_vars[first_var];
                    undef_vars[idx_end_var] = end_var;
                    idx_in_undef_vars[end_var] = idx_end_var;
                    if(undef_vars_sz==0){while (i < end) *j++=*i++; break;}
                }
            }
            ws.shrink(i-j);
        }

        if(undef_vars_sz==0) break;
        //  up cannot continue, random pick a var and assign.
        int add_var = undef_vars[rand()%undef_vars_sz];
        trl[trl_sz++] = mkLit(add_var,polarity[add_var]);
        tmp_up_build_soln[add_var] = !polarity[add_var];
        int end_var = undef_vars[--undef_vars_sz];
        int idx_end_var = idx_in_undef_vars[add_var];
        undef_vars[idx_end_var] = end_var;
        idx_in_undef_vars[end_var] = idx_end_var;
    }

    delete[] trl,undef_vars,idx_in_undef_vars;
    double tmp_time = cpuTime()-start_time;
    up_build_time += tmp_time;
    up_build_num++;
}

bool Solver::call_ls(build_type type){
    // double start_time = cpuTime();
    if(ccanr_has_constructed)reinit_CCAnr(lssolver);
    else init_CCAnr(lssolver),ccanr_has_constructed=true;
    load_ls_data();
    bool res = false;
    int  init_ls_unsat_ct;
    if(type == current_UP){
        build_soln_with_UP();
        settings(lssolver,tmp_up_build_soln);
    }else if(type == top_trail_UP){settings(lssolver,top_trail_soln);}
    else if(type == random_build){settings(lssolver,NULL);}
    init_ls_unsat_ct = lssolver->unsat_stack_fill_pointer;
    // double construct_time = cpuTime()-start_time;
    res = local_search(lssolver);
    confl_trans(lssolver);
    int ls_var_nums=nVars();
    for(int i=0;i<ls_var_nums;++i)ls_mediation_soln[i] = lssolver->best_soln[i+1];
    if(lssolver->best_cost <= ls_best_unsat_num){
        restarts_gap-=restarts_basic;
        if(restarts_gap<restarts_basic) restarts_gap = restarts_basic;
        ls_best_unsat_num = lssolver->best_cost;
        for(int i=0;i<ls_var_nums;++i) ls_best_soln[i] = ls_mediation_soln[i];
    }else{restarts_gap += restarts_basic;}
    freeze_ls_restart_num = restarts_gap;
    max_trail = max_trail*0.9;
    if(res==true) solved_by_ls = true;
    // double this_ls_time = cpuTime()-start_time;
    // using std::to_string;
    // std::cout   <<"c LS_res("<<to_string(++ls_call_num)+")="+to_string((int)res)
    //             <<",(cr="<<((trail.size()+0.0)/nVars())<<",pr="<<((trail.size()+0.0)/max_trail)<<")"
    //             <<",(fix"<<to_string(lssolver->fix_var_ct)+"v,del"+to_string(lssolver->del_cls_ct)+"c)"
    //             <<",(v="<<to_string(lssolver->num_vars)+",c="+to_string(lssolver->num_clauses)+")"
    //             <<",(unsat:"<<to_string(init_ls_unsat_ct)<<"->"<<to_string(lssolver->best_cost)+")"
    //             <<",("<<to_string(conflicts)<<"c,"<<to_string(starts)<<"r,"<<to_string(lssolver->step)<<"s)"
    //             <<",["<<to_string(max_trail)<<"trl,"<<to_string(up_build_num)<<"n,"<<to_string(up_build_time)<<"t]"
    //             <<","<<to_string(construct_time)<<","<<to_string(this_ls_time)
    //             <<std::endl;
    
    return res;
}
