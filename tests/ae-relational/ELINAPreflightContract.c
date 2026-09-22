/* Fixed-version ELINA preflight for the SVF relational backend adapter.
 *
 * This deliberately exercises only operations whose results have a small,
 * independent integer oracle.  It is not a substitute for the full adapter
 * differential suite.
 */

#include "elina_abstract0.h"
#include "opt_oct.h"
#include "opt_pk.h"

#include <fenv.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef elina_manager_t* (*manager_factory_t)(void);

static int failures;

static void check(bool condition, const char* domain, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL [%s] %s\n", domain, message);
        ++failures;
    }
}

static void check_no_exception(elina_manager_t* manager, const char* domain,
                               const char* operation)
{
    const elina_exc_t exception = manager->result.exn;
    if (exception != ELINA_EXC_NONE)
    {
        fprintf(stderr, "FAIL [%s] %s raised %s\n", domain, operation,
                elina_name_of_exception[exception]);
        ++failures;
    }
}

static elina_linexpr0_t* expression(size_t dimensions, int constant,
                                    int d0, int c0, int d1, int c1)
{
    elina_linexpr0_t* result =
        elina_linexpr0_alloc(ELINA_LINEXPR_DENSE, dimensions);
    elina_linexpr0_set_cst_scalar_int(result, constant);
    if (d0 >= 0)
        check(!elina_linexpr0_set_coeff_scalar_int(
                  result, (elina_dim_t)d0, c0),
              "preflight", "failed to set first expression coefficient");
    if (d1 >= 0)
        check(!elina_linexpr0_set_coeff_scalar_int(
                  result, (elina_dim_t)d1, c1),
              "preflight", "failed to set second expression coefficient");
    return result;
}

static elina_lincons0_array_t point_constraints(void)
{
    elina_lincons0_array_t constraints = elina_lincons0_array_make(2);
    constraints.p[0] = elina_lincons0_make(
        ELINA_CONS_EQ, expression(2, -5, 0, 1, -1, 0), NULL);
    constraints.p[1] = elina_lincons0_make(
        ELINA_CONS_EQ, expression(2, -2, 0, -1, 1, 1), NULL);
    return constraints;
}

static void check_bound(elina_manager_t* manager, elina_abstract0_t* value,
                        elina_dim_t dimension, int expected,
                        const char* domain, const char* message)
{
    elina_interval_t* interval =
        elina_abstract0_bound_dimension(manager, value, dimension);
    check_no_exception(manager, domain, message);
    check(interval != NULL, domain, "bound query returned null");
    if (interval != NULL)
    {
        check(elina_interval_equal_int(interval, expected), domain, message);
        elina_interval_free(interval);
    }
}

static void check_integer_strict_guard(elina_manager_t* manager,
                                       const char* domain)
{
    elina_abstract0_t* top = elina_abstract0_top(manager, 1, 0);
    elina_lincons0_array_t constraints = elina_lincons0_array_make(1);
    constraints.p[0] = elina_lincons0_make(
        ELINA_CONS_SUP, expression(1, 0, 0, 1, -1, 0), NULL);
    elina_abstract0_t* guarded = elina_abstract0_meet_lincons_array(
        manager, false, top, &constraints);
    check_no_exception(manager, domain, "integer strict guard");
    elina_interval_t* interval =
        elina_abstract0_bound_dimension(manager, guarded, 0);
    check_no_exception(manager, domain, "integer strict lower bound");
    check(interval != NULL, domain, "integer strict bound returned null");
    if (interval != NULL)
    {
        check(elina_scalar_cmp_int(interval->inf, 1) == 0, domain,
              "integer x > 0 did not tighten to lower bound 1");
        check(elina_scalar_infty(interval->sup) > 0, domain,
              "integer x > 0 unexpectedly gained a finite upper bound");
        elina_interval_free(interval);
    }
    elina_abstract0_free(manager, guarded);
    elina_abstract0_free(manager, top);
    elina_lincons0_array_clear(&constraints);
}

static void exercise_domain(elina_manager_t* manager, const char* domain,
                            bool octagon)
{
    check(manager != NULL, domain, "manager allocation failed");
    if (manager == NULL)
        return;

    printf("DOMAIN %s library=%s version=%s fenv=%d\n", domain,
           elina_manager_get_library(manager),
           elina_manager_get_version(manager), fegetround());

    elina_abstract0_t* top = elina_abstract0_top(manager, 2, 0);
    check_no_exception(manager, domain, "top");
    check(elina_abstract0_is_top(manager, top), domain, "top predicate");
    check_no_exception(manager, domain, "top predicate");

    elina_abstract0_t* bottom = elina_abstract0_bottom(manager, 2, 0);
    check_no_exception(manager, domain, "bottom");
    check(elina_abstract0_is_bottom(manager, bottom), domain,
          "bottom predicate");
    check_no_exception(manager, domain, "bottom predicate");

    elina_lincons0_array_t constraints = point_constraints();
    elina_abstract0_t* point = elina_abstract0_meet_lincons_array(
        manager, false, top, &constraints);
    check_no_exception(manager, domain, "meet point constraints");
    check_bound(manager, point, 0, 5, domain, "point x bound");
    check_bound(manager, point, 1, 7, domain, "point y bound");

    elina_lincons0_t relation = elina_lincons0_make(
        ELINA_CONS_EQ, expression(2, -2, 0, -1, 1, 1), NULL);
    check(elina_abstract0_sat_lincons(manager, point, &relation), domain,
          "point does not entail y - x = 2");
    check_no_exception(manager, domain, "relation entailment");
    elina_lincons0_clear(&relation);

    elina_linexpr0_t* increment = expression(2, 1, 0, 1, -1, 0);
    elina_abstract0_t* assigned = elina_abstract0_assign_linexpr(
        manager, false, point, 0, increment, NULL);
    check_no_exception(manager, domain, "assignment x := x + 1");
    check_bound(manager, assigned, 0, 6, domain, "assigned x bound");
    check_bound(manager, assigned, 1, 7, domain, "preserved y bound");
    elina_linexpr0_free(increment);

    elina_dim_t forgotten_dimension = 0;
    elina_abstract0_t* forgotten = elina_abstract0_forget_array(
        manager, false, point, &forgotten_dimension, 1, false);
    check_no_exception(manager, domain, "forget x");
    elina_interval_t* forgotten_x =
        elina_abstract0_bound_dimension(manager, forgotten, 0);
    check_no_exception(manager, domain, "forgotten x bound");
    check(forgotten_x != NULL && elina_interval_is_top(forgotten_x), domain,
          "forgotten x is not top");
    if (forgotten_x != NULL)
        elina_interval_free(forgotten_x);
    check_bound(manager, forgotten, 1, 7, domain, "forget preserved y");

    elina_dimchange_t add;
    elina_dimchange_init(&add, 1, 0);
    add.dim[0] = 2;
    elina_abstract0_t* extended = elina_abstract0_add_dimensions(
        manager, false, point, &add, false);
    check_no_exception(manager, domain, "add integer dimension");
    const elina_dimension_t extended_dimension =
        elina_abstract0_dimension(manager, extended);
    check(extended_dimension.intdim == 3 && extended_dimension.realdim == 0,
          domain, "added dimension count");
    elina_dimchange_clear(&add);

    elina_linexpr0_t* z_expression = expression(3, 1, 1, 1, -1, 0);
    elina_abstract0_t* z_assigned = elina_abstract0_assign_linexpr(
        manager, false, extended, 2, z_expression, NULL);
    check_no_exception(manager, domain, "assignment z := y + 1");
    check_bound(manager, z_assigned, 2, 8, domain, "assigned z bound");
    elina_linexpr0_free(z_expression);

    elina_dimchange_t remove;
    elina_dimchange_init(&remove, 1, 0);
    remove.dim[0] = 2;
    elina_abstract0_t* reduced = elina_abstract0_remove_dimensions(
        manager, false, z_assigned, &remove);
    check_no_exception(manager, domain, "remove integer dimension");
    const elina_dimension_t reduced_dimension =
        elina_abstract0_dimension(manager, reduced);
    check(reduced_dimension.intdim == 2 && reduced_dimension.realdim == 0,
          domain, "removed dimension count");
    check_bound(manager, reduced, 0, 5, domain, "remove preserved x");
    check_bound(manager, reduced, 1, 7, domain, "remove preserved y");
    elina_dimchange_clear(&remove);

    elina_abstract0_t* joined =
        elina_abstract0_join(manager, false, point, assigned);
    check_no_exception(manager, domain, "join");
    check(elina_abstract0_is_leq(manager, point, joined), domain,
          "join does not include left operand");
    check_no_exception(manager, domain, "left join inclusion");
    check(elina_abstract0_is_leq(manager, assigned, joined), domain,
          "join does not include right operand");
    check_no_exception(manager, domain, "right join inclusion");

    const bool top_leq_point =
        elina_abstract0_is_leq(manager, top, point);
    printf("LEQ_FALSE %s value=%d exact=%d best=%d exception=%s\n", domain,
           top_leq_point ? 1 : 0,
           elina_manager_get_flag_exact(manager) ? 1 : 0,
           elina_manager_get_flag_best(manager) ? 1 : 0,
           elina_name_of_exception[manager->result.exn]);

    elina_abstract0_t* closed =
        elina_abstract0_closure(manager, false, point);
    check_no_exception(manager, domain, "topological closure");
    check(elina_abstract0_is_eq(manager, point, closed), domain,
          "closure changed a closed point");
    check_no_exception(manager, domain, "closure equality");

    if (octagon)
    {
        elina_abstract0_minimize(manager, point);
        check(manager->result.exn == ELINA_EXC_NOT_IMPLEMENTED,
              domain, "Octagon minimize must be treated as unsupported");
        printf("EXPECTED_UNSUPPORTED %s operation=minimize exception=%s\n",
               domain,
               elina_name_of_exception[manager->result.exn]);
        elina_manager_clear_exclog(manager);
    }
    else
    {
        elina_abstract0_minimize(manager, point);
        check_no_exception(manager, domain, "Polyhedra minimize");
        elina_abstract0_canonicalize(manager, point);
        check_no_exception(manager, domain, "Polyhedra canonicalize");
    }

    check_integer_strict_guard(manager, domain);

    elina_abstract0_free(manager, closed);
    elina_abstract0_free(manager, joined);
    elina_abstract0_free(manager, reduced);
    elina_abstract0_free(manager, z_assigned);
    elina_abstract0_free(manager, extended);
    elina_abstract0_free(manager, forgotten);
    elina_abstract0_free(manager, assigned);
    elina_abstract0_free(manager, point);
    elina_lincons0_array_clear(&constraints);
    elina_abstract0_free(manager, bottom);
    elina_abstract0_free(manager, top);
    elina_manager_free(manager);
}

static elina_manager_t* make_octagon(void)
{
    return opt_oct_manager_alloc();
}

static elina_manager_t* make_polyhedra(void)
{
    /* strict=true exits the entire process in fixed ELINA f524156d. */
    return opt_pk_manager_alloc(false);
}

int main(void)
{
    exercise_domain(make_octagon(), "octagon", true);
    exercise_domain(make_polyhedra(), "polyhedra-loose", false);
    if (failures != 0)
    {
        fprintf(stderr, "ELINAPreflightContract: %d failure(s)\n", failures);
        return 1;
    }
    printf("ELINAPreflightContract: PASS\n");
    return 0;
}
